import { shouldBe } from "./resources/assert.js"

import('./different-view/main.js').then($vm.abort, function (error) {
    shouldBe(String(error), `SyntaxError: Export named 'A' cannot be resolved due to ambiguous multiple bindings in module '${import.meta.url.replace(/[^/]*$/, "different-view/A.js")}'.`);
}).catch($vm.abort);
