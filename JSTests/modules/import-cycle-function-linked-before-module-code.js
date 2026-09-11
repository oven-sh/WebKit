import { shouldBe } from "./resources/assert.js";
import * as namespace from "./import-cycle-function-linked-before-module-code/1.js";
import { lexical, hoisted, addLexical, readLexical, addLexicalThreeTimes, addHoisted, readHoisted } from "./import-cycle-function-linked-before-module-code/1.js";

// 2.js called addLexical() and addHoisted() while 1.js was not evaluated yet, so both functions were linked
// before the module code that declares the variables they write. Every read has to see every write, in every tier.
for (let i = 0; i < 1e5; ++i) {
    let base = i * 4;

    shouldBe(addLexical(1), base + 1);
    shouldBe(readLexical(), base + 1);
    shouldBe(lexical, base + 1);
    shouldBe(namespace.lexical, base + 1);

    shouldBe(addLexicalThreeTimes(), base + 4);
    shouldBe(readLexical(), base + 4);
    shouldBe(lexical, base + 4);
    shouldBe(namespace.lexical, base + 4);

    shouldBe(addHoisted(2), i * 2 + 2);
    shouldBe(readHoisted(), i * 2 + 2);
    shouldBe(hoisted, i * 2 + 2);
    shouldBe(namespace.hoisted, i * 2 + 2);
}
