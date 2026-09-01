// SCALEBENCH JS entry point (SPEC.md §5 file layout). The implementation lives
// in js/bench.js; this shim only forwards. "caller relative" makes the load
// independent of the runner's cwd. Shell-global `arguments` (W [, "smoke"]) is
// visible to the loaded file unchanged.
load("js/bench.js", "caller relative");
