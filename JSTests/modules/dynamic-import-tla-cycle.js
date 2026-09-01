import { shouldBe } from "./resources/assert.js";

let settled = 0;
setTimeout(() => {
    if (settled != 4)
        throw new Error("dynamic import() inside a TLA cycle deadlocked: " + settled + "/4 settled");
}, 1000);

for (const name of ["direct", "helper", "helper-after-await", "all"]) {
    const { ns } = await import("./dynamic-import-tla-cycle/entry-" + name + ".js");
    shouldBe(ns.v, name);
    ++settled;
}
