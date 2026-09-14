//@ requireOptions("--useJSThreads=1", "--useDollarVM=1", "--countJSThreadsCounters=1")
// The flag forces the FTL onto handler ICs. A handler IC whose call site can
// throw into a catch of the same machine frame was refused whenever the catch
// had a live value on the stack: the check counted a stack slot as the frame
// and stack pointers. Every access at that site then went to the C++ slow
// path. A property-adding helper called from a for-of loop is such a site - the
// loop's iterator-close handler is the catch (astar-like-nodes in the micro
// set). The check now ignores the frame and stack pointers, which no stub
// disturbs (SPEC-jit history §44). The catch still sees the right values.

function run() {
    function GridNode(x, y, w) { this.x = x; this.y = y; this.weight = w; }
    function clean(n) { n.f = 0; n.g = 0; n.h = 0; n.visited = false; n.closed = false; n.parent = null; }
    let keep = [];
    for (let i = 0; i < 4000; ++i)
        keep.push(new GridNode(i, i, 1));
    for (const node of keep)
        clean(node);
    let sum = 0;
    for (const node of keep)
        sum += node.x + node.f;
    return sum;
}

const before = $vm.jsThreadsCounter("icPutByIdGaveUp") || 0;
let expected = 0;
for (let i = 0; i < 4000; ++i)
    expected += i;
for (let round = 0; round < 60; ++round) {
    const sum = run();
    if (sum !== expected)
        throw new Error("round " + round + ": sum " + sum);
}
const gaveUp = ($vm.jsThreadsCounter("icPutByIdGaveUp") || 0) - before;
if (gaveUp > 10000)
    throw new Error(gaveUp + " put_by_id accesses went to the give-up slow path");

// The catch of such a site sees the values it had when the access threw.
function throwing() {
    const setterThrows = { set p(v) { throw new Error("setter " + v); } };
    const objects = [{}, {}, setterThrows, {}];
    let caught = 0;
    let marker = 7;
    for (let round = 0; round < 20000; ++round) {
        for (const o of objects) {
            try {
                marker = round & 15;
                o.p = round;
            } catch (e) {
                if (e.message !== "setter " + round || marker !== (round & 15))
                    throw new Error("catch saw " + e.message + " with marker " + marker + " in round " + round);
                ++caught;
            }
        }
    }
    return caught;
}
if (throwing() !== 20000)
    throw new Error("not every throwing set was caught");
