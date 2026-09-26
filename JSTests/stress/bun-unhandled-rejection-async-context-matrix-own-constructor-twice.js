//@ requireOptions("--useDollarVM=1")
//@ defaultNoEagerRun
load("./resources/bun-unhandled-rejection-async-context-matrix.js", "caller relative");

// One program in 31 of those with two derivations. 31 has no factor in common with the size of any dimension.
runMatrix({ kinds: ["ownConstructor"], depth: 2, stride: 31 });
