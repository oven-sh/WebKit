import { shouldBe } from "./resources/assert.js";
import { count, increment, object } from "./import-slots/values.js";
import * as staticNamespace from "./import-slots/values.js";

const namespace = await import("./import-slots/values.js");
shouldBe(namespace === staticNamespace, true);
shouldBe(namespace.object === object, true);
for (let i = 1; i <= testLoopCount; ++i) {
    namespace.increment();
    shouldBe(count, i);
    shouldBe(namespace.count, i);
    shouldBe(staticNamespace.count, i);
}
shouldBe(namespace.increment === increment, true);
// Imported for the first time dynamically, after this module started running.
const late = await import("./import-slots/late.js");
shouldBe(late.sometimes(true), "42,0,third:third");
const again = await import("./import-slots/late.js");
shouldBe(again === late, true);
