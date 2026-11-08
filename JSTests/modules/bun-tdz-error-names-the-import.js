// A failed op_check_tdz on an imported binding names the import as this module calls it. This module and early.js
// import each other, and early.js runs first: while it runs, the bindings of this module are in their dead zone.
import { checked } from "./bun-tdz-error-names-the-import/early.js";
import { shouldBe } from "./resources/assert.js";

export let counter = 0;
export class Api { }

shouldBe(checked, 8);
