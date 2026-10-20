//@ requireOptions("--useImportDefer=1")

// See import-evaluating-module-in-job-inside-evaluation.js. These graphs use `import defer` and `import.defer()`.

load("./resources/import-evaluating-module-in-job-inside-evaluation/harness.js", "caller relative");

// import.defer() does not evaluate the module: Link() is the only call for the module on the stack.
shouldBe(evaluate("import-defer", "a"), "b1 b2(a=fulfilled) a root | root=fulfilled a=fulfilled again=fulfilled");

// The first access to a deferred namespace evaluates its module: a second Evaluate() inside the walk. The end of
// that evaluation settles the import of the module that was on its stack (e.js), and not the import of the module
// that is on the stack of the outer walk (a.js).
shouldBe(evaluate("nested-evaluation", "a"), "b1 e(e=pending) d(a=pending,e=fulfilled) b2(a=pending,e=fulfilled) a root | root=fulfilled e=fulfilled a=fulfilled again=fulfilled");

// The inner evaluation fails: the import of the module that was on its stack rejects, and the import of the module
// on the stack of the outer walk still waits.
shouldBe(evaluate("nested-evaluation-fails", "a"), "b1 e(e=pending) caught(from e) b2(a=pending,e=rejected (from e)) a root | root=fulfilled a=fulfilled e=rejected (from e) again=fulfilled");

// The evaluation of a.js failed. Its deferred namespace gives that error, as each Evaluate() of a.js does.
shouldBe(evaluate("deferred-namespace-of-failed-module", "a"), "b1 b2(a=pending) a | root=rejected (from a) a=rejected (from a) again=rejected (from a)");
shouldBe(readDeferred(), "throws (from a)");
