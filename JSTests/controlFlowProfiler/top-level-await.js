load("./driver/driver.js");

// Must not crash: the basic block after a statement with an `await` starts at the end offset of the statement, which an `export` needs too.
var module;
import("./resources/top-level-await.js").then(result => { module = result; });
drainMicrotasks();
assert(module.a === 1 && module.default === 2 && module.c === 4, "the module ran to its end");
