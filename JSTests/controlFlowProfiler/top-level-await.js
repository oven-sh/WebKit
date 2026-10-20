load("./driver/driver.js");

// What is after a statement of a module with an `await` has a basic block, also when the statement is an `export`.
var module;
import("./resources/top-level-await.js").then(result => { module = result; });
drainMicrotasks();
assert(module.a === 1 && module.default === 2 && module.c === 4, "the module ran to its end");
