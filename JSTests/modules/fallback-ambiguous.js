//        +-----------------+
//        |                 |
//        v                 |
//       (A) -> (C) -> (D) *+-> [E]
//        *      ^
//        |      |
//        v      @
//       (B)
import { shouldBe } from "./resources/assert.js"

import('./fallback-ambiguous/main.js').then($vm.abort, function (error) {
    shouldBe(String(error), `SyntaxError: Cannot export 'A' multiple times in './D.js'`);
}).catch($vm.abort);
