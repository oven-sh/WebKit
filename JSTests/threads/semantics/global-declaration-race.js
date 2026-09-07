//@ requireOptions("--useJSThreads=1")
// Scripts evaluated on one global object from several threads at once. A
// script that declares a top-level `let`, `const` or `class` name that another
// script declared first must fail with a SyntaxError, exactly one declaration
// of each name must win, and `var` and function declarations of the same name
// from several scripts must all bind to the one global property. With the GIL
// off the declaration checks and the binding creation of two scripts must not
// interleave.
load("../harness.js", "caller relative");

const THREADS = 4;
const ROUNDS = 40;
const NAMES = 40; // lexical names per script, so that the check and the add take a while

function lexicalScript(i, t) {
    let text = "";
    for (let n = 0; n < NAMES; ++n)
        text += "let raceLet" + i + "_" + n + " = " + t + "; ";
    // Function declarations are bound between the lexical-name check and the
    // lexical-name creation, which is what makes the window wide.
    for (let n = 0; n < NAMES; ++n)
        text += "function raceFn" + i + "_" + n + "() { return " + t + "; } ";
    return text + "const raceConst" + i + " = " + t + "; class RaceClass" + i + " { static owner = " + t + "; }";
}

const gate = { go: 0 };
const threads = spawnN(THREADS, (t) => {
    while (Atomics.load(gate, "go") === 0) { }
    let won = 0;
    let duplicate = 0;
    let other = [];
    for (let i = 0; i < ROUNDS; ++i) {
        // Every thread races to declare the same lexical names for round i.
        try {
            $.evalScript(lexicalScript(i, t));
            ++won;
        } catch (e) {
            if (e instanceof SyntaxError)
                ++duplicate;
            else
                other.push(String(e));
        }
        // And var / function declarations of shared names, which never throw.
        $.evalScript("var raceVar" + i + " = " + t + "; function raceFunction" + i + "() { return " + t + "; }");
    }
    return { won, duplicate, other };
});
Atomics.store(gate, "go", 1);
const results = joinAll(threads);

let totalWon = 0;
for (let t = 0; t < THREADS; ++t) {
    shouldBe(results[t].other.length, 0, "unexpected errors on thread " + t + ": " + results[t].other.join("; "));
    shouldBe(results[t].won + results[t].duplicate, ROUNDS, "outcomes on thread " + t);
    totalWon += results[t].won;
}
// Exactly one thread won each name.
shouldBe(totalWon, ROUNDS, "declarations that succeeded, over all threads");

for (let i = 0; i < ROUNDS; ++i) {
    const owner = $.evalScript("raceLet" + i + "_0");
    shouldBe(typeof owner, "number", "type of raceLet" + i + "_0");
    for (let n = 1; n < NAMES; ++n)
        shouldBe($.evalScript("raceLet" + i + "_" + n), owner, "raceLet" + i + "_" + n + " agrees with _0");
    shouldBe($.evalScript("raceConst" + i), owner, "raceConst" + i + " agrees with raceLet" + i);
    shouldBe($.evalScript("RaceClass" + i + ".owner"), owner, "RaceClass" + i + " agrees with raceLet" + i);
    shouldBe(typeof globalThis["raceVar" + i], "number", "type of raceVar" + i);
    shouldBe(typeof globalThis["raceFunction" + i], "function", "type of raceFunction" + i);
    // A later script still cannot redeclare them, from any thread.
    shouldThrow(SyntaxError, () => $.evalScript("let raceLet" + i + "_0 = -1;"));
}

// Indirect eval declares vars and functions on the global object too.
const evalThreads = spawnN(THREADS, (t) => {
    for (let i = 0; i < ROUNDS; ++i)
        (0, eval)("var evalVar" + i + " = " + t + "; function evalFunction" + i + "() { return " + t + "; }");
    return true;
});
joinAll(evalThreads);
for (let i = 0; i < ROUNDS; ++i) {
    shouldBe(typeof globalThis["evalVar" + i], "number", "type of evalVar" + i);
    shouldBe(globalThis["evalFunction" + i](), globalThis["evalFunction" + i](), "evalFunction" + i + " is callable");
}
