//@ runDefault
//@ runDefault("--useSharedRegExpLiteralObjects=0")
//@ runDefault("--useSharedRegExpLiteralObjects=1")
//@ runDefault("--useSharedRegExpLiteralObjects=1", "--useJIT=0")
//@ runDefault("--useSharedRegExpLiteralObjects=1", "--useDFGJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10")
//@ runDefault("--useSharedRegExpLiteralObjects=1", "--useBytecodeOptimizer=1")

// RegExp.prototype.test converts its argument to a string before it looks at "exec". A toString that replaces "exec" runs inside
// the builtin, after the literal's object was made, after "test" was looked up and called: the replacement must be called on an
// object that belongs to this evaluation alone, for each of the evaluations that are in flight.
// (Only the interpreter and the Baseline JIT call the builtin here; the DFG's RegExpTest does not look at "exec" again after
// converting the argument, with or without this change.)

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + expected + " but got " + actual);
}

const originalExec = RegExp.prototype.exec;
let leaked = [];
let replace = false;

function probe(depth) {
    return /^[a-z]+$/.test({
        toString() {
            if (depth)
                return String(probe(depth - 1));
            if (replace)
                RegExp.prototype.exec = function (s) { leaked.push(this); return originalExec.call(this, s); };
            return "abc";
        }
    });
}
noInline(probe);

function optionalForms(s) {
    return [/^a/?.test(s), /^a/.test?.(s), (/^a/).test(s), /^(a)/.exec(s)?.[1], /^a/.exec?.(s)?.index].join();
}
noInline(optionalForms);

function* inGenerator(s) { return /^[a-z]+$/.test(yield s); }
async function inAsync(s) { return /^[a-z]+$/.test(await s); }

for (let i = 0; i < 400; i++) {
    shouldBe(probe(3), true);
    shouldBe(optionalForms("abc"), "true,true,true,a,0");
    shouldBe(optionalForms("xyz"), "false,false,false,,");
    let g = inGenerator("x");
    g.next();
    shouldBe(g.next("abc").value, true);
    let result;
    inAsync(Promise.resolve("a1")).then(v => result = v);
    drainMicrotasks();
    shouldBe(result, false);
}
shouldBe(leaked.length, 0);

replace = true;
let result = probe(3);
replace = false;
if ($vm.useDFGJIT && !$vm.useDFGJIT()) {
    // All four calls went through the builtin.
    shouldBe(leaked.length, 4);
}
shouldBe(result, true);
shouldBe(new Set(leaked).size, leaked.length, "one object per evaluation");
for (let r of leaked) {
    shouldBe(r.lastIndex, 0);
    shouldBe(r.source, "^[a-z]+$");
    shouldBe(Object.getOwnPropertyNames(r).join(), "lastIndex");
    r.lastIndex = 3;
    r.own = true;
}
let before = leaked.length;
for (let i = 0; i < 100; i++)
    shouldBe(probe(1), true);
shouldBe(leaked.length > before, true);
shouldBe(new Set(leaked).size, leaked.length);
shouldBe(leaked.slice(before).every(r => r.lastIndex === 0 && !("own" in r)), true);
RegExp.prototype.exec = originalExec;
