import { shouldBe } from "./resources/assert.js";
import { count, increment, set } from "./import-slots/values.js";
import { total, valuesNamespace, valuesDefault } from "./import-slots/reexport-named.js";
import { count as throughStar, own } from "./import-slots/reexport-star.js";
import { grandTotal, third } from "./import-slots/reexport-chain.js";
import { one } from "./import-slots/live-aliases.js";

// One variable seen through a direct import, a renaming re-export, a star re-export, a chain of
// both, a namespace re-export and another module's alias of it.
function all() { return [count, total, throughStar, grandTotal, valuesNamespace.count, one()].join(); }
for (let i = 1; i <= testLoopCount; ++i) {
    increment();
    shouldBe(all(), Array(6).fill(i).join());
}
set("reset");
shouldBe(all(), Array(6).fill("reset").join());
shouldBe([own, third, valuesDefault()].join(), "own,third:third,default");
