// ECMA-262 has no job that runs while a module graph is evaluated. A host can run one from a module body: here
// c.js and s.js call drainMicrotasks(), and the job that ends the top-level await of another module runs inside
// it. That job finishes (or rejects) a module while the modules that import it are in the middle of
// InnerModuleEvaluation or of AsyncModuleExecutionFulfilled.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: expected ${expected} but got ${actual}`);
}

// What the import of root.js gave and which bodies ran, after the jobs that were queued ran. When a module waits
// for `gate`, the same again after the gate opened.
function evaluate(name) {
    globalThis.log = [];
    globalThis.gate = new Promise(resolve => { globalThis.openGate = resolve; });
    let outcome = "pending";
    import(`./resources/module-finishes-in-job-inside-evaluation/${name}/root.js`).then(
        () => { outcome = "fulfilled"; },
        error => { outcome = `rejected (${error.message})`; });
    drainMicrotasks();
    const beforeGate = `${outcome}: ${log.join(" ")}`;
    openGate();
    drainMicrotasks();
    const afterGate = `${outcome}: ${log.join(" ")}`;
    return beforeGate === afterGate ? afterGate : `${beforeGate} | ${afterGate}`;
}

// The importer has not looked at its count of pending dependencies yet: it runs in the walk.
shouldBe(evaluate("fulfils"), "fulfilled: t1 c1 t2 c2 root");
shouldBe(evaluate("rejects"), "rejected (from t): t1 c1 t2 c2");

// The importer has looked at its count, and waits on the stack for a cycle that is not closed: it runs in the job.
shouldBe(evaluate("parent-in-open-cycle-fulfils"), "fulfilled: t1 c1 t2 p c2 root");
shouldBe(evaluate("parent-in-open-cycle-rejects"), "rejected (from t): t1 c1 t2 c2");
// Not before its other dependency has finished.
shouldBe(evaluate("parent-in-open-cycle-waits-for-two"), "pending: t1 u1 c1 t2 c2 | fulfilled: t1 u1 c1 t2 c2 u2 p root");
// When it has a top-level await of its own, the modules that import it wait for the end of its body.
shouldBe(evaluate("tla-parent-in-open-cycle"), "pending: t1 c1 t2 p1 c2 | fulfilled: t1 c1 t2 p1 c2 p2 root");

// A member of a cycle that is not closed ends its own async body.
shouldBe(evaluate("member-fulfils-on-stack"), "fulfilled: m1 c1 m2 c2 root");
shouldBe(evaluate("member-rejects-on-stack"), "rejected (from m): m1 c1 m2 c2");

// A module that the walk reaches later imports a member of the cycle that finished in the job.
shouldBe(evaluate("later-importer-of-finished-member"), "fulfilled: m1 c1 m2 c2 y root");
shouldBe(evaluate("later-importer-of-parent-run-in-job"), "fulfilled: t1 c1 t2 p c2 y root");
// When the job rejected that member, the later module does not run.
shouldBe(evaluate("later-importer-of-rejected-member"), "rejected (from m): m1 c1 m2 c2");
// Nor when the job rejected root.js, which waits for t.js and is in the middle of its walk.
shouldBe(evaluate("later-importer-of-rejected-importer"), "rejected (from t): t1 c1 t2 c2");

// The job loads and evaluates n.js, which imports root.js, a module on the stack of the walk.
shouldBe(evaluate("import-in-job-of-importer-of-module-on-stack"), "fulfilled: c1 n n fulfilled c2 root");
// When the job has rejected root.js before that, n.js does not run, and its import() rejects with the same error.
shouldBe(evaluate("import-in-job-of-importer-of-rejected-importer"), "rejected (from t): t1 c1 t2 n rejected (from t) c2");

// The rejection reaches a member of a cycle that is not closed.
shouldBe(evaluate("rejects-into-open-cycle"), "rejected (from t): t1 c1 t2 c2");

// The first rejection is the error of the importer, and a dependency that finishes later does not start it.
shouldBe(evaluate("two-dependencies-reject"), "rejected (from t): t1 u1 c1 t2 u2 c2");
shouldBe(evaluate("one-dependency-rejects-another-fulfils"), "rejected (from t): t1 u1 c1 t2 u2 c2");
// What the walk itself throws afterwards is the error of the modules on its stack.
shouldBe(evaluate("dependency-rejects-then-body-throws"), "rejected (from c): t1 c1 t2 c2");

// AsyncModuleExecutionFulfilled runs the body of s.js, and the job inside it finishes x.js, the other dependency of
// root.js. root.js runs when the body of s.js has ended, and not at all when that body throws.
shouldBe(evaluate("sibling-finishes-in-body-run-by-job"), "fulfilled: t1 t2 x1 s1 x2 s2 root");
shouldBe(evaluate("sibling-finishes-in-body-run-by-job-that-throws"), "rejected (from s): t1 t2 x1 s1 x2 s2");
