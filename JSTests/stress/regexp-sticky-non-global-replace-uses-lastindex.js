//@ runDefault("--useConcurrentJIT=0", "--thresholdForOptimizeAfterWarmUp=10", "--thresholdForFTLOptimizeAfterWarmUp=20")
// A sticky, non-global RegExp in String.prototype.replace matches at exactly lastIndex and updates it
// (RegExp.prototype[@@replace] -> RegExpBuiltinExec); the fast paths used to search from 0.
function shouldBe(actual, expected) { if (JSON.stringify(actual) !== JSON.stringify(expected)) throw new Error("expected " + JSON.stringify(expected) + " got " + JSON.stringify(actual)); }
function run() {
    const out = [];
    let re = /e/y; re.lastIndex = 1; out.push("eec".replace(re, "[$&]"), re.lastIndex);
    re = /(?:)/y; re.lastIndex = 1; out.push("axbx".replace(re, "[]"), re.lastIndex);
    re = /e/y; re.lastIndex = 1; out.push("eec".replace(re, (m) => "<" + m + ">"), re.lastIndex);
    re = /x/y; re.lastIndex = 1; out.push("eec".replace(re, "#"), re.lastIndex);
    re = /e/gy; re.lastIndex = 1; out.push("eece".replace(re, "#"), re.lastIndex);
    re = /e/y; re.lastIndex = 1; out.push("eec".replace(re, ""), re.lastIndex);
    re = /(?<x>e)/y; re.lastIndex = 2; out.push("eeec".replace(re, "$<x>!"), re.lastIndex);
    return out;
}
const expected = ["e[e]c", 2, "a[]xbx", 1, "e<e>c", 2, "eec", 0, "##ce", 0, "ec", 2, "eee!c", 3];
for (let i = 0; i < 3000; ++i)
    shouldBe(run(), expected);
function folded() { const q = /b/y; q.lastIndex = 1; return "abab".replace(q, "#") + q.lastIndex; }
for (let i = 0; i < 3000; ++i)
    shouldBe(folded(), "a#ab2");
{ let n = 0; const r = /b/y; r.lastIndex = 1; "abc".replace(r, { toString() { n++; return "X"; } }); shouldBe(n, 1); }
