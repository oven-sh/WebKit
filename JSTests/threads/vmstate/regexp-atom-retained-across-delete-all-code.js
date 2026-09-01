//@ requireOptions("--useJSThreads=1", "--useDollarVM=1")
// RegExp compile-state machine, flag-on: a RegExp's atom is set once by the
// first parse and survives deleteAllCode; bytecode is published before
// m_state names it. After $vm.deleteAllCodeWhenIdle() every kind of regexp
// (atom, JIT-able with captures, interpreter-only lookbehind) must recompile
// and produce the same results on the main thread and on a spawned thread,
// for both 8-bit and 16-bit subjects.
load("../resources/assert.js", "caller relative");

if (typeof $vm === "undefined")
    throw new Error("this test needs the jsc shell with --useDollarVM=1");

// Each run() evaluates the literals itself: the RegExp objects (and their
// lastIndex) are per call, while the compiled JSC::RegExp behind each
// pattern+flags pair is shared through the RegExpCache, which is what the
// test exercises across threads.
const subjects = [
    "xabcyabcabcz 12-ab 345-cd abbb ab",
    "xabcyabcabcz 12-ab 345-cd abbb ab \u0100\u1234",
];

function run(tag) {
    const atomGlobal = /abc/g;
    const atomSingle = /abc/;
    const captures = /(\d+)-(\w+)/g;
    const lookbehind = /(?<=a)b+/g;
    const out = [];
    for (const s of subjects) {
        atomGlobal.lastIndex = 0;
        out.push(JSON.stringify(s.match(atomGlobal)));
        out.push(JSON.stringify(atomSingle.exec(s)));
        out.push(String(atomSingle.test(s)));
        out.push(s.replace(atomGlobal, "_"));
        out.push(JSON.stringify(s.split(atomSingle)));
        captures.lastIndex = 0;
        let m;
        let caps = [];
        while ((m = captures.exec(s)))
            caps.push(m[1] + "/" + m[2] + "@" + m.index);
        out.push(caps.join(","));
        out.push(JSON.stringify(s.match(lookbehind)));
        out.push(s.replace(lookbehind, "B"));
    }
    return out.join("|");
}

const expected = [
    '["abc","abc","abc"]',
    '["abc"]',
    "true",
    "x_y__z 12-ab 345-cd abbb ab",
    '["x","y","","z 12-ab 345-cd abbb ab"]',
    "12/ab@13,345/cd@19",
    '["b","b","b","b","bbb","b"]',
    "xaBcyaBcaBcz 12-aB 345-cd aB aB",
    '["abc","abc","abc"]',
    '["abc"]',
    "true",
    "x_y__z 12-ab 345-cd abbb ab \u0100\u1234",
    '["x","y","","z 12-ab 345-cd abbb ab \u0100\u1234"]',
    "12/ab@13,345/cd@19",
    '["b","b","b","b","bbb","b"]',
    "xaBcyaBcaBcz 12-aB 345-cd aB aB \u0100\u1234",
].join("|");

shouldBe(run("before"), expected, "before deleteAllCode, main thread");
shouldBe(joinAll(spawnN(2, t => run("thread" + t)))[1], expected, "before deleteAllCode, spawned thread");

$vm.deleteAllCodeWhenIdle();

// The microtask runs after the script's entry scope pops, which is when
// deleteAllCodeWhenIdle fires. The jsc shell swallows exceptions thrown inside
// promise reactions, so the outcome is re-raised from the timer callback.
let ran = false;
let failure = null;
Promise.resolve().then(() => {
    try {
        shouldBe(run("after"), expected, "after deleteAllCode, main thread");
        const results = joinAll(spawnN(2, t => run("thread" + t)));
        shouldBe(results[0], expected, "after deleteAllCode, spawned thread 0");
        shouldBe(results[1], expected, "after deleteAllCode, spawned thread 1");
        ran = true;
    } catch (e) {
        failure = e;
    }
});

setTimeout(() => {
    if (failure)
        throw failure;
    if (!ran)
        throw new Error("post-deleteAllCode check did not run");
}, 0);
