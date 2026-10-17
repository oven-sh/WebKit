import { shouldBe } from "./resources/assert.js";

let settled = false;
setTimeout(() => {
    if (!settled)
        throw new Error("dynamic import() whose promise reaches the TLA referrer only through a captured resolver deadlocked");
}, 1000);

const { ns } = await import("./dynamic-import-tla-cycle/entry-opaque.js");
shouldBe(ns.v, "opaque");
settled = true;
