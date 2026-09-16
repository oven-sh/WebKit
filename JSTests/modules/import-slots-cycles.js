import { shouldBe } from "./resources/assert.js";
import { sawFromB, bValueSeen, viaB, setA, aValue } from "./import-slots/cycle-a.js";
import { earlyError, readA, b } from "./import-slots/cycle-b.js";
import { read, bump, value } from "./import-slots/self.js";

// cycle-b is evaluated first: it can call cycle-a's hoisted function but not read its `let`.
shouldBe(sawFromB, "b:a()");
shouldBe(bValueSeen, "bValue");
shouldBe(viaB, "a");
shouldBe(earlyError, "ReferenceError: Cannot access 'aValue' before initialization.");
for (let i = 0; i < testLoopCount; ++i) {
    setA(i);
    shouldBe(readA(), i);
    shouldBe(aValue, i);
    shouldBe(b(), "b:a()");
}
// A module that imports itself.
shouldBe(JSON.stringify(read()), "[1,1,true]");
shouldBe(bump(), 2);
shouldBe(JSON.stringify([read(), value]), "[[2,2,true],2]");
