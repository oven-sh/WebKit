import { addLexical, addHoisted } from "./1.js"
import { shouldBe, shouldThrow } from "../resources/assert.js";

// Module "1" is not evaluated yet. A call still links the function, which is all this needs:
// "lexical" is in its TDZ, and "hoisted" is already undefined.
shouldThrow(() => {
    addLexical(0);
}, `ReferenceError: Cannot access 'lexical' before initialization.`);
shouldBe(addHoisted(0), undefined);
