// Bun: upstream runs this in every mode. It expects the second match, the one that RegExp.lastMatch starts for the captures, to run out of stack.
// A flat program and the code of an image make that match within --maxRegExpStackSize, so nothing throws.
//@ $skipModes << "regexp-flat-program".to_sym
//@ $skipModes << "regexp-image-mode".to_sym
//@ requireOptions("--maxRegExpStackSize=65536")

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`expected ${expected} but got ${actual}`);
}

function shouldThrowRangeError(func) {
    let error;
    try {
        func();
    } catch (e) {
        error = e;
    }
    if (!(error instanceof RangeError))
        throw new Error(`expected RangeError but got ${error}`);
}

const string = "a".repeat(2000) + "c";
const regExp = new RegExp("(?:(?!(?:a|b)*d)a)*c" + "(x)?".repeat(1600));

if (regExp.test(string)) {
    for (const name of ["lastMatch", "lastParen", "$1", "leftContext", "rightContext"])
        shouldThrowRangeError(() => RegExp[name]);
    shouldThrowRangeError(() => { RegExp.input = "x"; });
    shouldBe(RegExp.input, string);
}

shouldBe(/b(c)d/.test("abcde"), true);
shouldBe(RegExp.lastMatch, "bcd");
shouldBe(RegExp.lastParen, "c");
shouldBe(RegExp.$1, "c");
shouldBe(RegExp.leftContext, "a");
shouldBe(RegExp.rightContext, "e");
shouldBe(RegExp.input, "abcde");
