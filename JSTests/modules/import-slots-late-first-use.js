import { shouldBe } from "./resources/assert.js";
import { sometimes, loop, increment, bump } from "./import-slots/late.js";

// Imports first read on a path that is only taken once the function is already hot, so the
// optimizing tiers meet a binding no lower tier has read.
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(sometimes(false), 0);
shouldBe(loop(testLoopCount, false), 0);
increment();
bump();
shouldBe(sometimes(true), "42,1,third:third");
shouldBe(loop(testLoopCount, true), "42,1,third:third");
for (let i = 0; i < testLoopCount; ++i) {
    bump();
    shouldBe(sometimes(i & 1), (i & 1) ? `42,${i + 2},third:third` : 1);
}
