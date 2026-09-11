//@ requireOptions("--useLazyModuleFunctionDeclarations=1")
// Two module records made from identical source text whose imports resolve to different kinds of bindings: code the
// tiers share between the CodeBlocks of one UnlinkedCodeBlock has to be right for both.
import * as a from "./lazy-function-declarations-siblings/a/user.js";
import * as b from "./lazy-function-declarations-siblings/b/user.js";
import { shouldBe } from "./resources/assert.js";

for (let i = 0; i < 2000; ++i) {
    shouldBe(a.readF()(), "declaration");
    shouldBe(a.callG(i), i + 1);
    shouldBe(a.typeofF(), "function");
}
for (let i = 0; i < 2000; ++i) {
    shouldBe(b.readF()(), "let");
    shouldBe(b.callG(i), i + 2);
    shouldBe(b.typeofF(), "function");
}
for (let i = 0; i < 100000; ++i) {
    shouldBe(a.callG(i) + b.callG(i), 2 * i + 3);
    shouldBe(a.readF() !== b.readF(), true);
}
