import { shouldBe } from "./resources/assert.js";
import { beforeBody, memberMeta, fromArrow, fromGenerator } from "./import-meta-first-evaluation/member.js";

// member.js is in a cycle with this module and runs first. It calls meta() before this body has run:
// that call is the first evaluation of import.meta of this module.
export function meta() {
    return import.meta;
}

shouldBe(typeof beforeBody, "object");
shouldBe(beforeBody, import.meta);
shouldBe(beforeBody.filename.endsWith("import-meta-first-evaluation.js"), true);
shouldBe(memberMeta.filename.endsWith("member.js"), true);
shouldBe(memberMeta === import.meta, false);
shouldBe(fromArrow(), memberMeta);
shouldBe(fromGenerator().next().value, memberMeta);

// Every later evaluation gives the same object, in every tier.
for (let i = 0; i < 1e5; ++i) {
    if (meta() !== beforeBody || import.meta !== beforeBody)
        throw new Error("bad value at " + i);
}
