//@ runDefault
//@ runDefault("--useSharedRegExpLiteralObjects=0")
//@ runDefault("--useSharedRegExpLiteralObjects=1", "--useJIT=0")
//@ runDefault("--useSharedRegExpLiteralObjects=1", "--useDFGJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForJITSoon=10")
//@ runDefault("--useSharedRegExpLiteralObjects=1", "--useConcurrentJIT=0", "--thresholdForJITAfterWarmUp=10", "--thresholdForOptimizeAfterWarmUp=20", "--thresholdForOptimizeAfterLongWarmUp=20")

// Several calls of /x/.test(...) from the same place are waiting for their argument when "exec" is replaced, and the same place
// is evaluated again (which tells the site that its object is not to be handed out any more) before any of them goes on.
// Each of them still has to show the replacement an object of its own.

function shouldBe(actual, expected, message) {
    if (actual !== expected)
        throw new Error((message ? message + ": " : "") + "expected " + expected + " but got " + actual);
}

const originalExec = RegExp.prototype.exec;
let leaked = [];

function nest(depth, replace) {
    return /^[a-z]+$/.test(depth ? String(nest(depth - 1, replace)) : (replace && (RegExp.prototype.exec = function (s) { leaked.push(this); return originalExec.call(this, s); }, nest(0, false), nest(1, false)), "abc"));
}
noInline(nest);

for (let i = 0; i < 3000; i++)
    shouldBe(nest(3, false), true);
shouldBe(nest(3, true), true);
// 4 waiting calls + the 3 evaluations made after the replacement.
shouldBe(leaked.length, 7);
shouldBe(new Set(leaked).size, 7, "one object per evaluation");
shouldBe(leaked.every(r => r.lastIndex === 0 && r.source === "^[a-z]+$" && Object.getOwnPropertyNames(r).join() === "lastIndex"), true);
RegExp.prototype.exec = originalExec;
