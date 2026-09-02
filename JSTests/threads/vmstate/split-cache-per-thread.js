//@ requireOptions("--useJSThreads=1")
// String.prototype.split and RegExp.prototype[Symbol.split] keep a result
// cache and an offsets scratch vector on the VM. Several threads split at
// once, each with its own strings and separators, so any sharing of those
// shows up as a wrong array.
load("../harness.js", "caller relative");

const THREADS = 4;
const ROUNDS = 150;

function atomize(s) {
    // Using a string as a property key makes its StringImpl an atom, and the
    // split cache only holds atom inputs.
    const o = {};
    o[s] = 1;
    return Object.keys(o)[0];
}

function makeCase(t, k) {
    const sep = [",", ";", "::", "|"][(t + k) % 4];
    const parts = [];
    for (let i = 0; i < 3 + ((t * 7 + k) % 9); ++i)
        parts.push("t" + t + "k" + k + "p" + i);
    return { input: atomize(parts.join(sep)), sep, parts };
}

function makeCases(t) {
    const cases = [];
    for (let k = 0; k < 12; ++k)
        cases.push(makeCase(t, k));
    return cases;
}

function expectedFor(cases) {
    // Computed before any thread starts, so the cache is cold and nothing
    // else is splitting.
    return cases.map(c => ({
        str: c.input.split(c.sep),
        re: c.input.split(new RegExp("(" + c.sep.replace("|", "\\|") + ")")),
    }));
}

const allCases = [];
const allExpected = [];
for (let t = 0; t < THREADS; ++t) {
    allCases.push(makeCases(t));
    allExpected.push(expectedFor(allCases[t]));
}

function sameArray(a, b) {
    if (a.length !== b.length)
        return false;
    for (let i = 0; i < a.length; ++i) {
        if (a[i] !== b[i])
            return false;
    }
    return true;
}

function work(t) {
    const cases = allCases[t];
    const expected = allExpected[t];
    const regexps = cases.map(c => new RegExp("(" + c.sep.replace("|", "\\|") + ")"));
    let bad = null;
    for (let round = 0; round < ROUNDS && !bad; ++round) {
        for (let k = 0; k < cases.length; ++k) {
            const c = cases[k];
            const s = c.input.split(c.sep);
            if (!sameArray(s, expected[k].str)) {
                bad = "thread " + t + " string split " + k + ": " + JSON.stringify(s);
                break;
            }
            const r = c.input.split(regexps[k]);
            if (!sameArray(r, expected[k].re)) {
                bad = "thread " + t + " regexp split " + k + ": " + JSON.stringify(r);
                break;
            }
        }
    }
    return bad;
}

const results = joinAll(spawnN(THREADS, work));
for (const r of results)
    shouldBe(r, null);

// The main thread's answers are unchanged afterwards.
shouldBe(work(0), null);
