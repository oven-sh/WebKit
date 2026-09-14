//@ threadsEnv("JSC_useJSThreads=1", "JSC_useSharedGCHeap=1", "JSC_useThreadGIL=0", "JSC_useThreadGILOffUnsafe=1")
//@ requireOptions("--useDollarVM=1")
//@ threadsForbidOutput("refusing GIL-off configuration")
// The GIL-off options given through the environment in an order where
// useThreadGIL=0 comes before the options that complete the shape. Options
// set during initialization (environment variables, an embedder's
// customization callback, which is where Bun applies its BUN_JSC_* options)
// used to run the GIL-off activation checklist after each one, so an
// intermediate state was refused with a "refusing GIL-off configuration" line
// and useThreadGIL forced back to 1; the shell then re-applied the environment
// and ended GIL off, an embedder without that second pass ended GIL on. The
// checklist now runs once, after the last option of the batch.

if ($vm.useThreadGIL())
    throw new Error("a complete GIL-off configuration given through the environment ended with the GIL on");
