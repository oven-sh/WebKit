import { shouldBe } from "./resources/assert.js";
import * as aliases from "./import-slots/aliases.js";
import * as shadow from "./import-slots/shadow.js";

for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(aliases.read(), `${i},${i},${i},${i},true`);
    aliases.increment();
    shouldBe(shadow.read("parameter"), "parameter,second:second,third:first");
    shouldBe(shadow.inner(), "second:first,local");
    shouldBe(shadow.globals(), "string,not the global Map,NaN-like,function");
}
shouldBe(typeof Map, "function");
