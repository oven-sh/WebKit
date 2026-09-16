import { shouldBe, shouldThrow } from "./resources/assert.js";
import * as assign from "./import-slots/assign.js";
import { count } from "./import-slots/values.js";

// An import is not assignable from the importing module, whatever it is bound to.
const readonly = "TypeError: Attempted to assign to readonly property.";
for (let i = 0; i < testLoopCount; ++i) {
    for (const name of ["assign", "increment", "compound", "destructuring", "objectDestructuring", "forIn", "forOf", "assignFunction", "assignDefault", "assignNamespace"])
        shouldThrow(assign[name], readonly);
    shouldThrow(assign.assignNamespaceProperty, readonly);
    assign.logical();
    shouldBe(assign.read(), 0);
    shouldBe(count, 0);
    shouldBe(assign.mutateObject(), true);
}
