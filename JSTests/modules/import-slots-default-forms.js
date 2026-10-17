import { shouldBe } from "./resources/assert.js";
import { read, rename, named } from "./import-slots/default-forms.js";
import theDefault from "./import-slots/default-named-function.js";

shouldBe(read(), "default,anonymous function,default,42,named,true,42");
shouldBe(named(), "named");
shouldBe(theDefault(), "named");
rename();
// `export default function named` exports the variable, so the reassignment is visible both ways.
shouldBe(read(), "default,anonymous function,default,42,replaced,true,42");
shouldBe(named(), "replaced");
shouldBe(theDefault(), "replaced");
