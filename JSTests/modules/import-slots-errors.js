import { shouldBe } from "./resources/assert.js";
import { count, increment } from "./import-slots/values.js";

async function rejection(specifier) {
    try {
        await import(specifier);
    } catch (error) {
        return error instanceof SyntaxError;
    }
    return "loaded";
}
// Linking fails the same way every time, and leaves the modules it shares with others usable.
for (let i = 0; i < 3; ++i) {
    shouldBe(await rejection("./import-slots/imports-missing.js"), true);
    shouldBe(await rejection("./import-slots/imports-conflict.js"), true);
}
shouldBe(increment(), 1);
shouldBe(count, 1);
const star = await import("./import-slots/star-conflict.js");
shouldBe(JSON.stringify(Object.keys(star).sort()), `["bump","live","second","third"]`);
shouldBe("first" in star, false);
