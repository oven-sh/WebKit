//@ runDefault("--compileMainScriptAheadOfTime=1", "--aotTypeCoveragePath=")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
const isInGenericCopy = 1, isInLoop = 4, isInlined = 8;
function operationsOf(f, opcode) {
    const lines = aotTypeCoverage(f.name);
    if (!lines) {
        if (isAOTCompiled(f))
            throw new Error("no coverage for " + f.name);
        return null;
    }
    return lines.map(line => {
        const [position, opcode, flags, timesRun, stubCalls, runtimeCalls, property, ...outcomes] = line.split(" ");
        return { position, opcode, flags: Number(flags), timesRun: Number(timesRun), stubCalls: Number(stubCalls), runtimeCalls: Number(runtimeCalls), property, outcomes };
    }).filter(operation => operation.opcode === opcode && !(operation.flags & isInGenericCopy));
}
function describe(operations) { return operations.map(operation => operation.position + " " + operation.property).join(); }

function readsTwo(o) { return o.first + o.second; }
function writesOne(o, v) { o.third = v; }
function testsFlag(o) { return o.flags & 4; }
function adds(a, b) { return a + b; }
function readsNothing(a) { return a; }
function usesThis(a) { "use strict"; return [this, a]; }
function readsOddNames(o) { return o["\t"] + o["a b"] + o["\n"] + o["\\"] + o["\u00e9"]; }
function loops(list) {
    let before = list.before;
    for (let i = 0; i < 10; i++)
        before += list.inside;
    return before;
}
function program() {
    function small(o) { return o.inner; }
    function caller(o) { return small(o) + o.outer; }
    noInline(caller);
    return caller;
}
function Point(x, y) { this.x = x; this.y = y; }
const caller = program();
for (const f of [readsTwo, writesOne, testsFlag, adds, readsNothing, usesThis, readsOddNames, loops])
    noInline(f);

check(readsTwo({ first: 1, second: 2 }), 3, "two reads");
check(testsFlag({ flags: 5 }), 4, "a flag");
check(adds(1, 2), 3, "a sum");
check(loops({ before: 1, inside: 2 }), 21, "a loop");
check(caller({ inner: 1, outer: 2 }), 3, "an inlined call");
check(new Point(1, 2).y, 2, "a constructor");

if (operationsOf(readsTwo, "op_get_by_id")) {
    check(describe(operationsOf(readsTwo, "op_get_by_id")), "21:32 first,21:42 second", "the reads of readsTwo");
    check(describe(operationsOf(readsTwo, "op_put_by_id")), "", "the writes of readsTwo");
    check(describe(operationsOf(writesOne, "op_put_by_id")), "22:29 third", "the writes of writesOne");
    check(describe(operationsOf(writesOne, "op_get_by_id")), "", "the reads of writesOne");
    check(describe(operationsOf(readsNothing, "op_get_by_id")), "", "the reads of readsNothing");
    check(aotTypeCoverage("noSuchFunction").length, 0, "the operations of a function that does not exist");
    check(operationsOf(readsOddNames, "op_get_by_id").map(read => read.property).join(), "\\u0009,a\\u0020b,\\u000a,\\u005c,\\u00e9", "names with characters that separate fields");
    check(operationsOf(readsNothing, "op_enter").map(enter => enter.outcomes.join()).join("; "), "not-in-graph", "an instruction that is not in the graph");
    check(operationsOf(readsTwo, "op_get_by_id").some(read => read.outcomes.includes("not-in-graph")), false, "an instruction that is in the graph");

    const [bitAnd] = operationsOf(testsFlag, "op_bitand");
    check(bitAnd.outcomes.includes("inline-bit-operation"), true, "a bit operation is inline");
    check(bitAnd.outcomes.some(outcome => outcome.startsWith("calls:")), false, "a bit operation calls nothing on its usual path");
    check(bitAnd.outcomes.some(outcome => outcome.startsWith("rarely-calls:")), true, "a bit operation calls something on its rare path");

    const [add] = operationsOf(adds, "op_add");
    check(add.outcomes.includes("inline-bit-operation"), false, "a sum is not a bit operation");

    const reads = operationsOf(loops, "op_get_by_id");
    check(operationsOf(readsTwo, "op_get_by_id").every(read => !(read.flags & isInLoop)), true, "a read in a function without a loop");
    check(reads.filter(read => read.property === "inside").every(read => read.flags & isInLoop), true, "a read in a loop");
    check(reads.some(read => read.property === "inside"), true, "the read in the loop is reported");

    check(describe(operationsOf(Point, "op_put_by_id").filter(write => write.outcomes.includes("absorbed-into-allocation"))), "40:28 x,40:40 y", "stores that an allocation absorbs");
    check(operationsOf(writesOne, "op_put_by_id").some(write => write.outcomes.includes("absorbed-into-allocation")), false, "a store to an object that exists already");

    check(operationsOf(readsTwo, "op_get_by_id").every(read => !read.timesRun && !read.stubCalls && !read.runtimeCalls), true, "nothing is counted unless asked for");

    const [toThis] = operationsOf(usesThis, "op_to_this");
    check(toThis.outcomes.includes("rarely-calls:operationAOTToThis"), true, "a call in a block that is branched to rarely is rare");
    check(toThis.outcomes.includes("calls:operationAOTToThis"), false, "and is not taken for one that is always made");
    check(operationsOf(usesThis, "op_new_array").every(allocation => allocation.outcomes.some(outcome => outcome.startsWith("calls:"))), true, "a call after an operation with a rare path is not rare");

    const readsOfCaller = operationsOf(caller, "op_get_by_id");
    check(describe(readsOfCaller.filter(read => read.flags & isInlined)), "35:33 inner", "a read in an inlined function has its own position");
    check(describe(readsOfCaller.filter(read => !(read.flags & isInlined))), "36:45 outer", "a read in the function itself");
}
