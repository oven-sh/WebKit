// ECMA-262 has no job that runs while a module graph is evaluated. A host can run one from a module body: here a
// body calls drainMicrotasks(), and the job that continues an import() runs inside it. That job calls Link() and
// Evaluate() for a module that is on the stack of the InnerModuleEvaluation that the body runs in.

load("./resources/import-evaluating-module-in-job-inside-evaluation/harness.js", "caller relative");

// The import is pending while the module is on the stack. It fulfils when the walk has finished the module.
shouldBe(evaluate("dependency", "a"), "b1 b2(a=pending) a root | root=fulfilled a=fulfilled a2=fulfilled again=fulfilled");
// A job that runs later inside the same walk sees it fulfilled.
shouldBe(evaluate("job-later-in-the-walk", "a"), "b1 b2(a=pending) a c(a=fulfilled) root | root=fulfilled a=fulfilled again=fulfilled");
// The module that Evaluate() was called for has a capability already.
shouldBe(evaluate("module-of-the-evaluate", "root"), "b1 b2(inner=pending) root | root=fulfilled inner=fulfilled again=fulfilled");

// A member of a cycle and the root of that cycle: both imports wait for the cycle.
shouldBe(evaluate("cycle-members", "c"), "c1 c2(c=pending,a=pending) a d(c=fulfilled,a=fulfilled) root | root=fulfilled c=fulfilled a=fulfilled again=fulfilled");
shouldBe(evaluate("cycle-with-the-module-of-the-evaluate", "a"), "a1 a2(a=pending,inner=pending) root | root=fulfilled a=fulfilled inner=fulfilled again=fulfilled");

// The module is asynchronous: the import waits for the end of its body, or for the modules that it waits for.
shouldBe(evaluate("top-level-await", "a"), "b1 b2(a=pending) a1 | root=pending a=pending -> b1 b2(a=pending) a1 a2 root | root=fulfilled a=fulfilled again=fulfilled");
// It settles before the import of root.js, which waits for a.js: the same order as for a job that runs after the walk.
shouldBe(settled.join(" "), "a root again");
shouldBe(evaluate("waits-for-async-dependency", "a"), "b1 b2(a=pending) t1 | root=pending a=pending -> b1 b2(a=pending) t1 t2 a root | root=fulfilled a=fulfilled again=fulfilled");
// A member of a cycle ends its async body before the root of the cycle has run: the import waits for the root.
shouldBe(evaluate("async-cycle-member", "c"), "c1 c2(c=pending) | root=pending c=pending -> c1 c2(c=pending) c3 a(c=pending) root | root=fulfilled c=fulfilled again=fulfilled");

// The evaluation fails while the module is on the stack: the import rejects with that error.
shouldBe(evaluate("body-throws", "a"), "b1 b2(a=pending) a | root=rejected (from a) a=rejected (from a) again=rejected (from a)");
shouldBe(evaluate("dependency-throws", "a"), "b1 b2(a=pending) c | root=rejected (from c) a=rejected (from c) again=rejected (from c)");
shouldBe(evaluate("cycle-root-body-throws", "c"), "c1 c2(c=pending) a | root=rejected (from a) c=rejected (from a) again=rejected (from a)");
shouldBe(evaluate("top-level-await-rejects", "a"), "b1 b2(a=pending) a1 | root=pending a=pending -> b1 b2(a=pending) a1 a2 | root=rejected (from a) a=rejected (from a) again=rejected (from a)");
// The evaluation fails after the walk has finished the module: the import fulfils.
shouldBe(evaluate("sibling-throws-later", "a"), "b1 b2(a=pending) a c | root=rejected (from c) a=fulfilled again=fulfilled");

// The promise of the Evaluate() for the module on the stack is alive in the VM only.
shouldBe(evaluate("collected-while-waiting", "a"), "b1 b2(a=pending) c a root | root=fulfilled a=fulfilled again=fulfilled");

// Two imports of one cycle, and a cycle whose root has the capability of the Evaluate(): the root is asynchronous.
shouldBe(evaluate("async-cycle-members", "c"), "c1 c2(c=pending,a=pending) a1 | root=pending c=pending a=pending -> c1 c2(c=pending,a=pending) a1 a2 root | root=fulfilled c=fulfilled a=fulfilled again=fulfilled");
shouldBe(evaluate("async-cycle-with-the-module-of-the-evaluate", "a"), "a1 a2(a=pending) root1 | root=pending a=pending -> a1 a2(a=pending) root1 root2 | root=fulfilled a=fulfilled again=fulfilled");

// An import whose job runs after the evaluation has failed gets the same error.
shouldBe(evaluate("import-started-before-the-failure", "a"), "b1 b2(a=pending,a2=pending) a | root=rejected (from a) a=rejected (from a) a2=rejected (from a) again=rejected (from a)");

// A module body waits for the import. It can: the module that it imports does not wait for that body.
shouldBe(evaluate("awaited-by-later-module", "a"), "b1 b2(a=pending) a y1 y2 root | root=fulfilled a=fulfilled again=fulfilled");
// It cannot: the module that it imports waits for that body. An import() after the walk gives the same.
shouldBe(evaluate("awaited-by-dependency", "a"), "b1 b2(a=pending) | root=pending a=pending again=pending");
shouldBe(evaluate("awaited-by-dependency-of-cycle-root", "c"), "c1 x1 x2(c=pending) | root=pending c=pending -> c1 x1 x2(c=pending) c2 | root=pending c=pending again=pending");

// Not changed: a module that imports the module on the stack does not wait for it (the import of x.js runs x.js in
// the job, before a.js has finished).
shouldBe(evaluate("importer-of-module-on-the-stack", "x"), "b1 x b2(x=fulfilled) a root | root=fulfilled x=fulfilled again=fulfilled");

// A job finishes or rejects a module that is on the stack, and a job imports that module.
shouldBe(evaluate("finishes-on-the-stack", "c"), "c1 c2(c=pending) c3 d(c=pending) a root | root=fulfilled c=fulfilled again=fulfilled");
shouldBe(evaluate("imported-after-it-finished-on-the-stack", "c"), "c1 c2 d(c=pending) a root | root=fulfilled c=fulfilled again=fulfilled");
shouldBe(evaluate("rejects-on-the-stack", "c"), "c1 c2(c=pending) c3 d(c=pending) | root=rejected (from c) c=rejected (from c) again=rejected (from c)");
shouldBe(evaluate("rejects-on-the-stack-then-body-throws", "c"), "c1 c2(c=pending) c3 d(c=pending) | root=rejected (from d) c=rejected (from d) again=rejected (from d)");
shouldBe(evaluate("imported-after-a-job-rejected-it", "root"), "t1 b1 t2 b2(inner=pending) | root=rejected (from b) inner=rejected (from b) again=rejected (from b)");
