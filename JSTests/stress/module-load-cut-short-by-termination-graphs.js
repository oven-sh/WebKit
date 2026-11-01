//@ runDefault("--useDollarVM=1", "--useJIT=0")

// As module-load-cut-short-by-termination.js, for more graphs: a chain whose middle module is loaded again first, a
// diamond, a cycle, a graph with a top-level await, and a graph whose root awaits its own import().

load("./resources/module-load-cut-short-by-termination/walk.js");

walk("chain", { begin: beginImport, check: checkImports, leaf: "mid.js" });
walk("diamond", { begin: beginImport, check: checkImports });
walk("cycle", { begin: beginImport, check: checkImports });
walk("await", { begin: beginImport, check: checkImports });
walk("aimp", { begin: beginImport, check: checkImports });
