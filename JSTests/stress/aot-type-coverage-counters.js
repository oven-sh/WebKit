//@ runDefault("--compileMainScriptAheadOfTime=1", "--aotTypeCoveragePath=", "--useAOTTypeCoverageCounters=1")
function check(actual, expected, what) {
    if (!Object.is(actual, expected))
        throw new Error(what + ": " + String(actual) + " instead of " + String(expected));
}
const isInGenericCopy = 1;
function operationsOf(f, opcode) {
    const lines = aotTypeCoverage(f.name);
    if (!lines) {
        if (isAOTCompiled(f))
            throw new Error("no coverage for " + f.name);
        return null;
    }
    return lines.map(line => {
        const [position, opcode, flags, timesRun, rareCalls, property, ...outcomes] = line.split(" ");
        return { position, opcode, flags: Number(flags), timesRun: Number(timesRun), rareCalls: Number(rareCalls), property, outcomes };
    }).filter(operation => operation.opcode === opcode);
}
function timesRun(f, opcode, property = "") {
    const counts = new Set(operationsOf(f, opcode).filter(operation => operation.property === property).map(operation => operation.timesRun));
    check(counts.size, 1, "every copy of " + opcode + " " + property + " in " + f.name + " has the same counter");
    return [...counts][0];
}
function rareCalls(f, opcode) {
    return Math.max(...operationsOf(f, opcode).map(operation => operation.rareCalls));
}

function reads(o) { return o.value; }
function isNeverCalled(o) { return o.value; }
function branches(o, which) {
    if (which)
        return o.taken;
    return o.other;
}
function testsFlag(o) { return o.flags & 4; }
function loops(o, n) {
    let sum = 0;
    for (let i = 0; i < n; i++)
        sum += o.inside;
    return sum;
}
function Point(x, y) { this.x = x; this.y = y; }
function program() {
    function small(o) { return o.inner; }
    function first(o) { return small(o) + 1; }
    function second(o) { return small(o) + 2; }
    noInline(first);
    noInline(second);
    return [first, second];
}
const [first, second] = program();
for (const f of [reads, isNeverCalled, branches, testsFlag, loops, Point])
    noInline(f);

if (operationsOf(reads, "op_get_by_id")) {
    check(timesRun(reads, "op_get_by_id", "value"), 0, "before the first call");
    for (let i = 0; i < 7; i++)
        reads({ value: i });
    check(timesRun(reads, "op_get_by_id", "value"), 7, "after seven calls");
    check(timesRun(reads, "op_ret"), 7, "an operation of another kind");
    check(timesRun(isNeverCalled, "op_get_by_id", "value"), 0, "a function that is never called");

    for (let i = 0; i < 5; i++)
        branches({ taken: 1, other: 2 }, i < 2);
    check(timesRun(branches, "op_get_by_id", "taken"), 2, "one side of a branch");
    check(timesRun(branches, "op_get_by_id", "other"), 3, "the other side");

    for (let i = 0; i < 10; i++)
        testsFlag({ flags: i });
    check(timesRun(testsFlag, "op_bitand"), 10, "a bit operation on int32s");
    check(rareCalls(testsFlag, "op_bitand"), 0, "int32s stay on the usual path");
    for (let i = 0; i < 3; i++)
        testsFlag({ flags: "4" });
    check(timesRun(testsFlag, "op_bitand"), 13, "a bit operation on strings too");
    check(rareCalls(testsFlag, "op_bitand") >= 3, true, "strings leave the usual path");

    check(loops({ inside: 2 }, 6), 12, "a loop");
    check(operationsOf(loops, "op_get_by_id").reduce((most, read) => Math.max(most, read.timesRun), 0), 6, "an operation in a loop, in whichever copy ran");

    for (let i = 0; i < 4; i++)
        new Point(i, i);
    check(timesRun(Point, "op_create_this"), 4, "an allocation");
    check(operationsOf(Point, "op_put_by_id").filter(write => write.outcomes.includes("absorbed-into-allocation")).map(write => write.timesRun).join(), "4,4", "the stores it absorbs");

    for (let i = 0; i < 2; i++)
        first({ inner: i });
    for (let i = 0; i < 3; i++)
        second({ inner: i });
    for (const f of [first, second])
        check(timesRun(f, "op_get_by_id", "inner"), 5, "an operation that is inlined twice has one counter, seen from " + f.name);
}
