//@ requireOptions("--useJSThreads=1")
// An inline cache case is generated only after couldStillSucceed() re-checked
// its property conditions; single-threaded nothing can change between that
// check and generation, and the compiler release-asserts if a condition no
// longer holds ("This condition is no longer met"). With JS threads another
// thread can transition the condition's object - a shared prototype - in that
// window. Flag-on the case is now given up instead (the next repatch re-derives
// it). Found by the mirror harness (stress/primitive-poly-proto.js, rare).
// This test keeps one thread generating miss/hit ICs across a prototype chain
// while another keeps reshaping the prototypes.
load("../harness.js", "caller relative");

const protoA = { pa: 1 };
const protoB = Object.create(protoA); protoB.pb = 2;
function makeObj(i) { const o = Object.create(protoB); o.own = i; return o; }

const box = { stop: 0 };
const mutator = new Thread(() => {
    let n = 0;
    // do/while: GIL on, this thread may first run when main is already
    // joining (stop set); it still does one round.
    do {
        // Reshape both prototypes: add and delete properties (structure
        // transitions, dictionary conversions), toggle the looked-up names.
        protoA["k" + (n & 15)] = n; delete protoA["k" + ((n + 8) & 15)];
        if (n & 1) { protoB.pa = 7; } else { delete protoB.pa; }
        if (n & 2) { protoA.missing = 1; } else { delete protoA.missing; }
        n++;
    } while (!Atomics.load(box, "stop"));
    return n;
});

// Many distinct access sites so IC generation keeps happening; each site is a
// fresh function so its IC starts empty.
let sum = 0;
const deadline = Date.now() + 2000;
let sites = 0;
while (Date.now() < deadline) {
    const f = new Function("o", "let s = 0; for (let i = 0; i < 40; ++i) { s += (o.pa|0) + (o.pb|0) + (o.missing === undefined ? 0 : 1) + o.own; } return s;");
    const o = makeObj(sites & 7);
    sum += f(o);
    sites++;
}
Atomics.store(box, "stop", 1);
const n = mutator.join();
if (!(sites > 0 && n > 0)) throw new Error("did not run");
print("PASS");
