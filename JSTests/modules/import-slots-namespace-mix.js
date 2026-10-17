import { shouldBe } from "./resources/assert.js";
import * as first from "./import-slots/mix-first.js";
import * as middle from "./import-slots/mix-middle.js";
import * as last from "./import-slots/mix-last.js";
import * as only from "./import-slots/only-namespaces.js";

// Namespace imports before, between and after the named ones.
shouldBe(first.read(), "0,0,second:first,true");
shouldBe(middle.read(), "0,0,second:first,second:first,true");
shouldBe(last.read(), "0,0,second:first,third:third,true,true");
shouldBe(only.read(), "0,second:first");
for (let i = 1; i <= testLoopCount; ++i) {
    [first, middle, last, only][i % 4].increment();
    shouldBe(first.read(), `${i},${i},second:first,true`);
    shouldBe(middle.read(), `${i},${i},second:first,second:first,true`);
    shouldBe(last.read(), `${i},${i},second:first,third:third,true,true`);
    shouldBe(only.read(), `${i},second:first`);
}
