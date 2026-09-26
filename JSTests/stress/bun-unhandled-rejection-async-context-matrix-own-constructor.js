//@ requireOptions("--useDollarVM=1")
//@ defaultNoEagerRun
load("./resources/bun-unhandled-rejection-async-context-matrix.js", "caller relative");

runMatrix({ kinds: ["ownConstructor"] });
