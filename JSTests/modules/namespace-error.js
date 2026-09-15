import { shouldBe } from "./resources/assert.js"

import('./namespace-error/namespace-local-error-should-hide-global-ambiguity.js').then($vm.abort, function (error) {
    shouldBe(String(error), `SyntaxError: export default cannot be used with export *`);
}).catch($vm.abort);
