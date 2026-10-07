//@ runDefault("--maxRegExpStackSize=65536", "--maxPerThreadStackUsage=1572864")

// Reading a legacy static (RegExp.lastMatch, RegExp.$1) runs the last search again, now with its
// subpatterns. JIT code keeps one context for each iteration of the group on the machine stack,
// and a context is 8 bytes larger for each subpattern: this search needs about 0.7 MB of the
// 1.5 MB stack when test() runs it, and about 6 MB when it runs again. The interpreter then has
// no room in its pool either. The second run used to come back as "no match", and the getter
// read an array that was not there.

function shouldBe(actual, expected, what) {
    if (actual !== expected)
        throw new Error(what + ": expected " + JSON.stringify(expected) + " but got " + JSON.stringify(actual));
}

const regExp = new RegExp("^" + "(z)?".repeat(32) + "(?:a|b)+$");
const subject = "a".repeat(20000);

for (const read of [() => RegExp.lastMatch, () => RegExp.$1, () => RegExp.lastParen, () => RegExp["$&"], () => RegExp.leftContext, () => RegExp.rightContext]) {
    shouldBe(regExp.test(subject), true, "test");
    let error = null;
    try {
        read();
    } catch (e) {
        error = e;
    }
    shouldBe(error instanceof RangeError, true, String(read));
}

// The statics work again after a search that fits both times.
shouldBe(regExp.test("ab"), true, "short test");
shouldBe(RegExp.lastMatch, "ab", "short lastMatch");
shouldBe(RegExp.$1, "", "short $1");
