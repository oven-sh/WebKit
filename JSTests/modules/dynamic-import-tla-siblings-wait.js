import { shouldBe } from "./resources/assert.js";

const [A, B] = await Promise.all([
    import("./dynamic-import-tla-cycle/sibling-a.js"),
    import("./dynamic-import-tla-cycle/sibling-b.js"),
]);
shouldBe(A.a, true);
shouldBe(B.b, true);
