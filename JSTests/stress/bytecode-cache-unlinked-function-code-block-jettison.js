//@ runBytecodeCache("--useUnlinkedCodeBlockJettisoning=1", "--useUnlinkedCodeBlockJettisoningForBytecodeCache=1", "--unlinkedCodeBlockJettisonAge=0", "--forceCodeBlockToJettisonDueToOldAge=1")

// On the second run every function below is decoded from the bytecode cache.
// forceCodeBlockToJettisonDueToOldAge drops linked CodeBlocks at every full GC, after which the
// decoded UnlinkedFunctionCodeBlocks are only weakly referenced and (with jettison age 0) die at
// the next full GC. Their owning executables must then re-materialize working code on demand,
// over and over.
//
// Every repeatedly-called function here is flat (no nested functions) and top-level: the
// cache-writing first run jettisons too, and re-encoding a function whose record nests other
// executables is not supported once jettisoned children can be collected (stale entries in the
// cache's leaf-executable map can collide with reused cells).

var sum = 0;

function add(x) { sum += x; return sum; }

function Point(x, y) {
    if (!new.target)
        return "called:" + x;
    this.x = x;
    this.y = y;
}
Point.prototype.len2 = function len2() { return this.x * this.x + this.y * this.y; };

function fib(n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }

// Called a single time, before any jettison, and never compiled again: its record nests a child
// executable, so its jettisoned code block exercises the return to the cached representation for
// records with children.
function makeCounter() {
    var count = 0;
    return function counter() { return ++count; };
}

var counter = makeCounter();
if (counter() !== 1 || counter() !== 2)
    throw new Error("counter broke");
counter = null;

function check(iteration) {
    sum = 0;
    if (add(3) !== 3 || add(4) !== 7)
        throw new Error("add broke at iteration " + iteration);
    var p = new Point(3, 4);
    if (p.len2() !== 25)
        throw new Error("construct broke at iteration " + iteration);
    if (Point(7) !== "called:7")
        throw new Error("call broke at iteration " + iteration);
    if (fib(10) !== 55)
        throw new Error("fib broke at iteration " + iteration);
}

check(-1);
for (var i = 0; i < 20; ++i) {
    fullGC();
    if (i % 3 === 0)
        edenGC();
    check(i);
}
