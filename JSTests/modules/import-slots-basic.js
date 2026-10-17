import { shouldBe } from "./resources/assert.js";
import defaultFunction, { count, increment, set, object, constant, variable, declared, Klass } from "./import-slots/values.js";
import { first, second } from "./import-slots/second.js";
import { first as thirdFirst, third } from "./import-slots/third.js";
import thirdDefault from "./import-slots/third.js";

// Every kind of export, read at the top level and from functions.
shouldBe(JSON.stringify([count, constant, variable, declared(), Klass.tag, defaultFunction(), object.tag]), `[0,42,"var","declared","class","default","object"]`);
shouldBe([first, second, thirdFirst, third, thirdDefault.tag].join(), "second:first,second:second,third:first,third:third,third:default");

function read() { return count; }
for (let i = 0; i < testLoopCount; ++i) {
    shouldBe(increment(), i + 1);
    shouldBe(read(), i + 1);
    shouldBe(count, i + 1);
}
set(-1);
shouldBe(read(), -1);
shouldBe(typeof increment, "function");
shouldBe(typeof count, "number");
