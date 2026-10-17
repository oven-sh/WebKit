// ECMA-262 has no job that runs while InnerModuleEvaluation walks a module graph. A host can run one from a module
// body: here c.js calls drainMicrotasks(), and the job that ends the top-level await of a module that the walk
// reached before runs inside it. That job finishes (or rejects) a module while the modules that import it are
// still on the stack of the walk.

function shouldBe(actual, expected) {
    if (actual !== expected)
        throw new Error(`bad value: expected ${expected} but got ${actual}`);
}

function evaluate(name) {
    globalThis.log = [];
    let outcome = "pending";
    import(`./resources/module-finishes-in-job-inside-walk/${name}/root.js`).then(
        () => { outcome = "fulfilled"; },
        error => { outcome = `rejected (${error.message})`; });
    drainMicrotasks();
    return `${outcome}: ${log.join(" ")}`;
}

// The importer has not looked at its count of pending dependencies yet: it runs in the walk.
shouldBe(evaluate("fulfils"), "fulfilled: t1 c1 t2 c2 root");
shouldBe(evaluate("rejects"), "rejected (from t): t1 c1 t2 c2");

// The importer has looked at its count, and waits on the stack for a cycle that is not closed: it runs in the job.
shouldBe(evaluate("parent-in-open-cycle-fulfils"), "fulfilled: t1 c1 t2 p c2 root");
shouldBe(evaluate("parent-in-open-cycle-rejects"), "rejected (from t): t1 c1 t2 c2");

// A member of a cycle that is not closed ends its own async body.
shouldBe(evaluate("member-fulfils-on-stack"), "fulfilled: m1 c1 m2 c2 root");
shouldBe(evaluate("member-rejects-on-stack"), "rejected (from m): m1 c1 m2 c2");

// A module that the walk reaches after the job rejected its dependency does not run.
shouldBe(evaluate("later-importer-of-rejected-member"), "rejected (from m): m1 c1 m2 c2");

// The rejection reaches a member of a cycle that is not closed.
shouldBe(evaluate("rejects-into-open-cycle"), "rejected (from t): t1 c1 t2 c2");
