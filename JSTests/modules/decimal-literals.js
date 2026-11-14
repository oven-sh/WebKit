//@ requireOptions("--useDecimal=1")
import { shouldBe } from "./resources/assert.js";
import * as self from "./decimal-literals.js";

export const named = 1.5m;
export default .5m;
const awaited = await 2m;

shouldBe(String(named), "1.5");
shouldBe(String(awaited), "2");
shouldBe(String(self.named.add(awaited)), "3.5");
shouldBe(String(self.default), "0.5");
