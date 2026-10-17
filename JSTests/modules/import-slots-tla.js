import { shouldBe } from "./resources/assert.js";
import { seenAtStart, seenAfterAwait, read } from "./import-slots/tla-main.js";
import { count } from "./import-slots/values.js";

shouldBe(JSON.stringify(seenAtStart), `["ready","after"]`);
shouldBe(JSON.stringify(seenAfterAwait), `["ready","after",1]`);
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(JSON.stringify(read()), `["ready",1]`);
shouldBe(count, 1);
