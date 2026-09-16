import { shouldBe } from "./resources/assert.js";
import { evaluated, onlySecond, neverCalled } from "./import-slots/unused.js";
import { count } from "./import-slots/values.js";

// A module whose imports are never read, or first read long after it was evaluated.
shouldBe(evaluated, true);
for (let i = 0; i < testLoopCount; ++i)
    shouldBe(onlySecond(), "second:second");
fullGC();
const values = neverCalled();
shouldBe(values.length, 10);
shouldBe(values[0], count);
shouldBe(values[8], "second:first");
shouldBe(values[6].constant, 42);
