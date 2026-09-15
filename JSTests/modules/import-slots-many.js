import { shouldBe } from "./resources/assert.js";
import { sum, bumpEverything, first, last } from "./import-slots/many.js";

// 128 imported bindings from 8 modules in one module.
let expected = 0;
for (let m = 0; m < 8; ++m) {
    for (let i = 0; i < 16; ++i)
        expected += m * 100 + i;
}
shouldBe(sum(), expected);
shouldBe(first(), 0);
shouldBe(last(), 715);
for (let i = 1; i <= testLoopCount; ++i) {
    bumpEverything();
    shouldBe(sum(), expected + 128 * i);
}
shouldBe(first(), testLoopCount);
shouldBe(last(), 715 + testLoopCount);
