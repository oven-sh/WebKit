import { shouldBe } from "./resources/assert.js"

import('./different-view/main.js').then($vm.abort, function (error) {
    // This fork's message names the module by its URL; compare with the directory taken off.
    shouldBe(String(error).replace(/file:\/\/.*\//, ""), `SyntaxError: Export named 'A' cannot be resolved due to ambiguous multiple bindings in module 'A.js'.`);
}).catch($vm.abort);
