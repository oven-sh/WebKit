import "./namespace-export-of-module-not-linked-yet/b.js";
import { read } from "./namespace-export-of-module-not-linked-yet/a.js";
import { shouldBe } from "./resources/assert.js";

// a.js and b.js import each other, and a.js is linked first. Its import of `ns` is the namespace of c.js, which b.js
// requests after a.js: InnerModuleLinking has not visited c.js yet when a.js takes the namespace object.
shouldBe(read(), "v: c");
