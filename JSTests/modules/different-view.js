import { shouldBe } from "./resources/assert.js"

import('./different-view/main.js').then($vm.abort, function (error) {
    shouldBe(String(error), `SyntaxError: Export named 'A' cannot be resolved due to ambiguous multiple bindings in module '${import.meta.url.slice(0, import.meta.url.lastIndexOf("/") + 1)}different-view/A.js'.`);
}).catch($vm.abort);
